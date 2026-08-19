#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <pulp/signal/scope_capture.hpp>
#include <pulp/signal/spectrum_trace.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <span>
#include <type_traits>

using namespace pulp::signal;
using Catch::Matchers::WithinAbs;

namespace {

auto single_band_config(double sample_rate, std::uint32_t fft_size, double low, double high) {
    SpectrumTraceConfig config;
    config.sample_rate = sample_rate;
    config.fft_size = fft_size;
    config.band_count = 1;
    config.minimum_hz = low;
    config.maximum_hz = high;
    config.band_scale = SpectrumBandScale::linear;
    config.attack = 1.0;
    config.release = 1.0;
    return config;
}

template <typename Capture>
void feed_partitioned(Capture& capture, std::span<const double> input,
                      std::span<const std::size_t> partitions) {
    std::size_t position = 0;
    for (const auto size : partitions) {
        capture.process(input.subspan(position, size));
        position += size;
    }
    REQUIRE(position == input.size());
}

} // namespace

static_assert(std::is_trivially_copyable_v<SpectrumTrace64>);
static_assert(std::is_trivially_copyable_v<ScopeCapture64>);
static_assert(sizeof(SpectrumTrace64) < 65536);
static_assert(sizeof(ScopeCapture64) < 140000);

TEST_CASE("Small-capacity analysis front ends are valid immediately",
          "[signal][analysis-frontends][fault]") {
    SpectrumTraceT<float, 2, 1> spectrum;
    const std::array<float, 2> bins{};
    REQUIRE(spectrum.input_bin_count() == bins.size());
    REQUIRE(spectrum.process_frame(bins));
    REQUIRE(spectrum.current().band_count == 1);

    ScopeCaptureT<float, 1> scope;
    REQUIRE(scope.config().capture_samples == 1);
    REQUIRE(scope.config().pretrigger_samples == 0);
    const std::array<float, 2> samples{{-1.0F, 0.0F}};
    scope.process(samples);
    REQUIRE(scope.capture_ready());
    REQUIRE(scope.capture().sample_count == 1);
}

TEST_CASE("Spectrum trace averages linear power rather than decibels",
          "[signal][analysis-frontends][spectrum]") {
    SpectrumTraceT<double, 5, 2> trace;
    auto config = single_band_config(8000.0, 8, 500.0, 2500.0);
    REQUIRE(trace.configure(config));
    const std::array<double, 5> bins{{-120.0, 0.0, -10.0, -120.0, -120.0}};
    REQUIRE(trace.process_frame(bins));

    const double expected = 10.0 * std::log10((1.0 + 0.1) / 2.0);
    REQUIRE_THAT(trace.current().magnitude_db[0], WithinAbs(expected, 1.0e-12));
    REQUIRE(std::abs(trace.current().magnitude_db[0] - (-5.0)) > 2.0);
    REQUIRE(trace.current().sequence == 1);
    REQUIRE(trace.algorithmic_latency_samples() == 0);

    config.aggregation = SpectrumBandAggregation::maximum;
    REQUIRE(trace.configure(config));
    REQUIRE(trace.process_frame(bins));
    REQUIRE(trace.current().magnitude_db[0] == 0.0);
}

TEST_CASE("Spectrum weighting follows standard reference points and stays finite",
          "[signal][analysis-frontends][spectrum][oracle]") {
    SpectrumTraceT<double, 5, 1> trace;
    auto config = single_band_config(8000.0, 8, 999.0, 1001.0);
    config.weighting = SpectrumFrequencyWeighting::a;
    REQUIRE(trace.configure(config));
    const std::array<double, 5> unity{{0.0, 0.0, 0.0, 0.0, 0.0}};
    REQUIRE(trace.process_frame(unity));
    REQUIRE_THAT(trace.current().magnitude_db[0], WithinAbs(0.0, 0.05));

    config = single_band_config(800.0, 8, 99.0, 101.0);
    config.weighting = SpectrumFrequencyWeighting::a;
    REQUIRE(trace.configure(config));
    REQUIRE(trace.process_frame(unity));
    REQUIRE_THAT(trace.current().magnitude_db[0], WithinAbs(-19.1, 0.15));

    for (const auto weighting : {SpectrumFrequencyWeighting::a, SpectrumFrequencyWeighting::c}) {
        config.minimum_hz = 0.0;
        config.maximum_hz = 1.0;
        config.floor_db = -500.0;
        config.weighting = weighting;
        REQUIRE(trace.configure(config));
        auto hot_dc = unity;
        hot_dc[0] = config.ceiling_db;
        REQUIRE(trace.process_frame(hot_dc));
        REQUIRE(std::isfinite(trace.current().magnitude_db[0]));
        REQUIRE(trace.current().magnitude_db[0] == config.floor_db);
    }

    config = single_band_config(252.0, 8, 31.4, 31.6);
    config.weighting = SpectrumFrequencyWeighting::c;
    REQUIRE(trace.configure(config));
    REQUIRE(trace.process_frame(unity));
    REQUIRE_THAT(trace.current().magnitude_db[0], WithinAbs(-3.03, 0.08));

    config = single_band_config(20000.0, 8, 2499.0, 2501.0);
    config.weighting = SpectrumFrequencyWeighting::a;
    REQUIRE(trace.configure(config));
    auto positive_weighting = unity;
    positive_weighting[1] = -120.5;
    REQUIRE(trace.process_frame(positive_weighting));
    REQUIRE_THAT(trace.current().magnitude_db[0], WithinAbs(-119.23, 0.08));
    positive_weighting[1] = std::numeric_limits<double>::quiet_NaN();
    REQUIRE(trace.process_frame(positive_weighting));
    REQUIRE(trace.current().magnitude_db[0] == config.floor_db);
}

TEST_CASE("Spectrum bands use only FFT centers inside their authored edges",
          "[signal][analysis-frontends][spectrum][oracle][fault]") {
    SpectrumTrace64 defaults;
    std::array<double, 513> default_bins{};
    default_bins.fill(defaults.config().floor_db);
    default_bins[0] = defaults.config().ceiling_db;
    REQUIRE(defaults.process_frame(default_bins));
    for (std::size_t band = 0; band < defaults.current().band_count; ++band)
        REQUIRE(defaults.current().magnitude_db[band] == defaults.config().floor_db);

    SpectrumTraceT<double, 5, 1> endpoints;
    auto config = single_band_config(8000.0, 8, 0.0, 1.0);
    REQUIRE(endpoints.configure(config));
    std::array<double, 5> bins{};
    bins.fill(config.floor_db);
    bins[0] = 0.0;
    REQUIRE(endpoints.process_frame(bins));
    REQUIRE(endpoints.current().magnitude_db[0] == 0.0);

    config = single_band_config(8000.0, 8, 3999.0, 4000.0);
    REQUIRE(endpoints.configure(config));
    bins.fill(config.floor_db);
    bins[4] = 0.0;
    REQUIRE(endpoints.process_frame(bins));
    REQUIRE(endpoints.current().magnitude_db[0] == 0.0);
}

TEST_CASE("Spectrum smoothing preserves legacy coefficients and peak-frame timing",
          "[signal][analysis-frontends][spectrum][timing]") {
    SpectrumTraceT<double, 5, 1> trace;
    auto config = single_band_config(8000.0, 8, 999.0, 1001.0);
    config.attack = 0.5;
    config.release = 0.15;
    config.peak_hold_frames = 1;
    config.peak_decay_db_per_frame = 2.0;
    REQUIRE(trace.configure(config));

    // Expectations are DERIVED from the configured coefficients, not pasted. The
    // smoothing law is one-pole toward the target, selecting attack when rising
    // and release when falling, so a reference that mirrors it turns this into a
    // check of the coefficients actually in force. Written as literals, the
    // assertions still passed when a coefficient changed meaning.
    const auto smooth = [&](double previous, double target) {
        const double coefficient = target > previous ? config.attack : config.release;
        return previous + coefficient * (target - previous);
    };
    // Peak-hold decays one step per frame and is re-armed whenever the magnitude
    // rises above it. Asserting the decayed value alone silently assumed the
    // rebound always overtakes it — true for these coefficients, false for
    // others, which is exactly the coupling a derived expectation removes.
    const auto decayed_peak = [&](double previous_peak, double magnitude) {
        return std::max(previous_peak - config.peak_decay_db_per_frame, magnitude);
    };
    constexpr double kTolerance = 1.0e-12;

    std::array<double, 5> bins{};
    REQUIRE(trace.process_frame(bins));
    const double settled = trace.current().magnitude_db[0];

    bins[1] = -20.0;
    REQUIRE(trace.process_frame(bins));
    const double falling_once = smooth(settled, -20.0);
    REQUIRE_THAT(trace.current().magnitude_db[0], WithinAbs(falling_once, kTolerance));
    REQUIRE(trace.current().peak_db[0] == 0.0);

    REQUIRE(trace.process_frame(bins));
    const double falling_twice = smooth(falling_once, -20.0);
    REQUIRE_THAT(trace.current().magnitude_db[0], WithinAbs(falling_twice, kTolerance));
    const double peak_after_decay = decayed_peak(0.0, falling_twice);
    REQUIRE_THAT(trace.current().peak_db[0], WithinAbs(peak_after_decay, kTolerance));

    bins[1] = 0.0;
    REQUIRE(trace.process_frame(bins));
    const double rising = smooth(falling_twice, 0.0);
    REQUIRE_THAT(trace.current().magnitude_db[0], WithinAbs(rising, kTolerance));
    REQUIRE_THAT(trace.current().peak_db[0],
                 WithinAbs(decayed_peak(peak_after_decay, rising), kTolerance));
}

TEST_CASE("Spectrum faults are finite transactional and reset deterministic",
          "[signal][analysis-frontends][spectrum][fault]") {
    SpectrumTraceT<double, 5, 2> trace;
    auto config = single_band_config(8000.0, 8, 500.0, 2500.0);
    REQUIRE(trace.configure(config));
    const std::array<double, 5> bins{
        {-120.0, 0.0, std::numeric_limits<double>::quiet_NaN(), -120.0, -120.0}};
    REQUIRE(trace.process_frame(bins));
    const auto first = trace.current();
    REQUIRE(std::isfinite(first.magnitude_db[0]));

    const std::array<double, 4> short_frame{};
    REQUIRE_FALSE(trace.process_frame(short_frame));
    REQUIRE(trace.current().sequence == first.sequence);
    auto invalid = config;
    invalid.sample_rate = std::numeric_limits<double>::infinity();
    REQUIRE_FALSE(trace.configure(invalid));
    REQUIRE(trace.config().sample_rate == config.sample_rate);
    REQUIRE(trace.current().sequence == first.sequence);

    trace.reset();
    REQUIRE(trace.process_frame(bins));
    REQUIRE(trace.current().magnitude_db == first.magnitude_db);
    REQUIRE(trace.current().peak_db == first.peak_db);

    auto extreme = single_band_config(8000.0, 8, 0.0, 4000.0);
    extreme.aggregation = SpectrumBandAggregation::maximum;
    extreme.floor_db = std::numeric_limits<double>::lowest();
    extreme.ceiling_db = std::numeric_limits<double>::max();
    extreme.attack = 0.5;
    extreme.release = 0.5;
    extreme.peak_decay_db_per_frame = std::numeric_limits<double>::max();
    REQUIRE(trace.configure(extreme));
    std::array<double, 5> extreme_bins{};
    extreme_bins.fill(std::numeric_limits<double>::max());
    REQUIRE(trace.process_frame(extreme_bins));
    extreme_bins.fill(std::numeric_limits<double>::lowest());
    REQUIRE(trace.process_frame(extreme_bins));
    REQUIRE(std::isfinite(trace.current().magnitude_db[0]));
    REQUIRE(std::isfinite(trace.current().peak_db[0]));
    REQUIRE_THAT(trace.current().magnitude_db[0], WithinAbs(0.0, 0.0));

    extreme.attack = 1.0;
    extreme.release = 1.0;
    REQUIRE(trace.configure(extreme));
    extreme_bins.fill(-1.0);
    REQUIRE(trace.process_frame(extreme_bins));
    extreme_bins.fill(-2.0);
    REQUIRE(trace.process_frame(extreme_bins));
    REQUIRE(std::isfinite(trace.current().magnitude_db[0]));
    REQUIRE(std::isfinite(trace.current().peak_db[0]));
}

TEST_CASE("Scope capture preserves exact pretrigger trigger and posttrigger samples",
          "[signal][analysis-frontends][scope][oracle]") {
    ScopeCaptureT<double, 16> scope;
    ScopeCaptureConfig config;
    config.capture_samples = 5;
    config.pretrigger_samples = 2;
    config.trigger_mode = ScopeTriggerMode::rising_edge;
    config.trigger_level = 0.0;
    config.hysteresis = 0.1;
    REQUIRE(scope.configure(config));

    const std::array<double, 8> input{{-2.0, -1.0, -0.5, 0.5, 1.0, 2.0, 3.0, 4.0}};
    scope.process(input);
    REQUIRE(scope.capture_ready());
    const auto& capture = scope.capture();
    REQUIRE(capture.trigger_sample_index == 3);
    REQUIRE(capture.trigger_offset == 2);
    REQUIRE(scope.capture_completion_latency_samples() == 2);
    const std::array<double, 5> expected{{-1.0, -0.5, 0.5, 1.0, 2.0}};
    REQUIRE(std::equal(expected.begin(), expected.end(), capture.samples.begin()));

    scope.arm();
    const std::array<double, 4> second_input{{-1.0, 1.0, 2.0, 3.0}};
    scope.process(second_input);
    REQUIRE(scope.capture_ready());
    REQUIRE(scope.capture().generation == 2);
}

TEST_CASE("Scope continuous holdoff ignores exactly the authored sample count",
          "[signal][analysis-frontends][scope][timing]") {
    ScopeCaptureT<double, 16> scope;
    ScopeCaptureConfig config;
    config.capture_samples = 3;
    config.pretrigger_samples = 0;
    config.trigger_mode = ScopeTriggerMode::rising_edge;
    config.trigger_level = 0.0;
    config.hysteresis = 0.5;
    config.holdoff_samples = 2;
    config.continuous = true;
    REQUIRE(scope.configure(config));

    const std::array<double, 10> input{{-1.0, 1.0, 2.0, 3.0, -1.0, 1.0, -1.0, 1.0, 4.0, 5.0}};
    scope.process(std::span<const double>{input}.first(6));
    REQUIRE(scope.capture().generation == 1);
    REQUIRE(scope.holdoff_remaining() == 0);
    REQUIRE(scope.armed());
    scope.process(std::span<const double>{input}.subspan(6));
    REQUIRE(scope.capture().generation == 2);
    REQUIRE(scope.capture().trigger_sample_index == 7);
    const std::array<double, 3> expected{{1.0, 4.0, 5.0}};
    REQUIRE(std::equal(expected.begin(), expected.end(), scope.capture().samples.begin()));
}

TEST_CASE("Scope continuous capture keeps the last complete frame while reacquiring",
          "[signal][analysis-frontends][scope][timing][regression]") {
    ScopeCaptureT<double, 8> scope;
    ScopeCaptureConfig config;
    config.capture_samples = 3;
    config.pretrigger_samples = 0;
    config.trigger_mode = ScopeTriggerMode::immediate;
    config.continuous = true;
    REQUIRE(scope.configure(config));

    const std::array<double, 6> input{{0.0, 1.0, 2.0, 3.0, 4.0, 5.0}};
    scope.process(std::span<const double>{input}.first(4));
    REQUIRE(scope.capture_ready());
    REQUIRE(scope.capturing());
    REQUIRE(scope.capture().generation == 1);
    REQUIRE(scope.capture().trigger_sample_index == 0);
    REQUIRE(std::equal(input.begin(), input.begin() + 3, scope.capture().samples.begin()));

    scope.process(std::span<const double>{input}.last(2));
    REQUIRE(scope.capture_ready());
    REQUIRE(scope.armed());
    REQUIRE(scope.capture().generation == 2);
    REQUIRE(scope.capture().trigger_sample_index == 3);
    REQUIRE(std::equal(input.begin() + 3, input.end(), scope.capture().samples.begin()));
}

TEST_CASE("Scope capture is bit-identical across input block partitions",
          "[signal][analysis-frontends][scope][determinism]") {
    ScopeCaptureT<double, 16> whole;
    ScopeCaptureT<double, 16> partitioned;
    ScopeCaptureConfig config;
    config.capture_samples = 7;
    config.pretrigger_samples = 3;
    config.trigger_mode = ScopeTriggerMode::falling_edge;
    config.trigger_level = 0.25;
    config.hysteresis = 0.05;
    REQUIRE(whole.configure(config));
    REQUIRE(partitioned.configure(config));

    const std::array<double, 12> input{
        {0.7, 0.6, 0.5, 0.4, 0.2, 0.1, -0.1, -0.2, -0.3, -0.4, -0.5, -0.6}};
    whole.process(input);
    constexpr std::array<std::size_t, 5> partitions{{1, 2, 4, 3, 2}};
    feed_partitioned(partitioned, input, partitions);
    REQUIRE(whole.capture_ready());
    REQUIRE(partitioned.capture_ready());
    REQUIRE(whole.capture().samples == partitioned.capture().samples);
    REQUIRE(whole.capture().trigger_sample_index == partitioned.capture().trigger_sample_index);
    REQUIRE(whole.capture().trigger_offset == partitioned.capture().trigger_offset);
    REQUIRE(whole.samples_seen() == partitioned.samples_seen());

    const auto first_capture = whole.capture();
    whole.reset();
    whole.process(input);
    REQUIRE(whole.capture().samples == first_capture.samples);
    REQUIRE(whole.capture().trigger_sample_index == first_capture.trigger_sample_index);
    REQUIRE(whole.capture().generation == first_capture.generation);
}

TEST_CASE("Scope nonfinite samples sanitize without creating a false edge",
          "[signal][analysis-frontends][scope][fault]") {
    ScopeCaptureT<double, 8> scope;
    ScopeCaptureConfig config;
    config.capture_samples = 3;
    config.pretrigger_samples = 1;
    config.trigger_mode = ScopeTriggerMode::rising_edge;
    config.hysteresis = 0.1;
    REQUIRE(scope.configure(config));

    const std::array<double, 4> input{{-1.0, std::numeric_limits<double>::quiet_NaN(), 1.0, 2.0}};
    scope.process(input);
    REQUIRE(scope.capture_ready());
    REQUIRE(scope.capture().trigger_sample_index == 2);
    REQUIRE(scope.capture().nonfinite_samples_seen == 1);
    REQUIRE(scope.capture().samples[0] == 0.0);
    REQUIRE(scope.capture().samples[1] == 1.0);
    REQUIRE(scope.capture().samples[2] == 2.0);

    ScopeCaptureT<double, 1> immediate;
    config.capture_samples = 1;
    config.pretrigger_samples = 0;
    config.trigger_mode = ScopeTriggerMode::immediate;
    REQUIRE(immediate.configure(config));
    const std::array<double, 1> immediate_input{
        {std::numeric_limits<double>::infinity()}};
    immediate.process(immediate_input);
    REQUIRE(immediate.capture_ready());
    REQUIRE(immediate.capture().trigger_sample_index == 0);
    REQUIRE(immediate.capture().nonfinite_samples_seen == 1);
    REQUIRE(immediate.capture().samples[0] == 0.0);

    ScopeCaptureT<double, 4> immediate_with_history;
    config.capture_samples = 4;
    config.pretrigger_samples = 2;
    REQUIRE(immediate_with_history.configure(config));
    const std::array<double, 4> nonfinite_window{
        {std::numeric_limits<double>::quiet_NaN(),
         std::numeric_limits<double>::infinity(),
         -std::numeric_limits<double>::infinity(),
         std::numeric_limits<double>::quiet_NaN()}};
    immediate_with_history.process(std::span<const double>{nonfinite_window}.first(3));
    REQUIRE_FALSE(immediate_with_history.capture_ready());
    REQUIRE(immediate_with_history.capturing());
    REQUIRE(immediate_with_history.capture_completion_latency_samples() == 1);
    immediate_with_history.process(std::span<const double>{nonfinite_window}.last(1));
    REQUIRE(immediate_with_history.capture_ready());
    REQUIRE(immediate_with_history.capture().trigger_sample_index == 2);
    REQUIRE(immediate_with_history.capture().trigger_offset == 2);
    REQUIRE(immediate_with_history.capture().nonfinite_samples_seen == 4);
    REQUIRE(immediate_with_history.capture().samples == std::array<double, 4>{});

    const auto before = scope.capture();
    auto invalid = config;
    invalid.pretrigger_samples = invalid.capture_samples;
    REQUIRE_FALSE(scope.configure(invalid));
    REQUIRE(scope.capture().samples == before.samples);
    REQUIRE(scope.capture().generation == before.generation);
}

TEST_CASE("Scope edge triggers require a prior hysteresis-side sample",
          "[signal][analysis-frontends][scope][oracle][fault]") {
    ScopeCaptureT<float, 8> scope;
    ScopeCaptureConfig config;
    config.capture_samples = 2;
    config.pretrigger_samples = 0;
    config.trigger_mode = ScopeTriggerMode::either_edge;
    REQUIRE(scope.configure(config));

    const std::array<float, 4> constant_at_level{};
    scope.process(constant_at_level);
    REQUIRE_FALSE(scope.capture_ready());

    const std::array<float, 3> rising{{-1.0F, 1.0F, 2.0F}};
    scope.process(rising);
    REQUIRE(scope.capture_ready());
    REQUIRE(scope.capture().trigger_sample_index == 5);

    config.trigger_mode = ScopeTriggerMode::falling_edge;
    config.hysteresis = 0.25;
    REQUIRE(scope.configure(config));
    const std::array<float, 4> falling{{1.0F, 0.5F, -0.5F, -1.0F}};
    scope.process(falling);
    REQUIRE(scope.capture_ready());
    REQUIRE(scope.capture().trigger_sample_index == 2);

    ScopeCaptureT<float, 8> startup;
    config.capture_samples = 4;
    config.pretrigger_samples = 3;
    config.trigger_mode = ScopeTriggerMode::rising_edge;
    config.hysteresis = 0.0;
    REQUIRE(startup.configure(config));
    const std::array<float, 4> crossing_before_history{{-1.0F, 1.0F, 1.0F, 1.0F}};
    startup.process(crossing_before_history);
    REQUIRE_FALSE(startup.capture_ready());
    const std::array<float, 2> fresh_crossing{{-1.0F, 1.0F}};
    startup.process(fresh_crossing);
    REQUIRE(startup.capture_ready());
    REQUIRE(startup.capture().trigger_sample_index == 5);
    REQUIRE(startup.capture().trigger_offset == 3);
}

TEST_CASE("Float analysis front ends reject narrowing overflow transactionally",
          "[signal][analysis-frontends][fault]") {
    SpectrumTraceT<float, 513, 4> spectrum;
    const auto spectrum_before = spectrum.config();
    auto spectrum_invalid = spectrum_before;
    spectrum_invalid.floor_db = -std::numeric_limits<double>::max();
    REQUIRE_FALSE(spectrum.configure(spectrum_invalid));
    spectrum_invalid = spectrum_before;
    spectrum_invalid.sample_rate = 384001.0;
    REQUIRE_FALSE(spectrum.configure(spectrum_invalid));
    REQUIRE(spectrum.config().sample_rate == spectrum_before.sample_rate);

    auto spectrum_extreme = spectrum_before;
    spectrum_extreme.band_count = 1;
    spectrum_extreme.aggregation = SpectrumBandAggregation::maximum;
    spectrum_extreme.floor_db = std::numeric_limits<float>::lowest();
    spectrum_extreme.ceiling_db = std::numeric_limits<float>::max();
    spectrum_extreme.attack = 0.5;
    spectrum_extreme.release = 0.5;
    spectrum_extreme.peak_decay_db_per_frame = std::numeric_limits<float>::max();
    REQUIRE(spectrum.configure(spectrum_extreme));
    std::array<float, 513> extreme_bins{};
    extreme_bins.fill(std::numeric_limits<float>::max());
    REQUIRE(spectrum.process_frame(extreme_bins));
    extreme_bins.fill(std::numeric_limits<float>::lowest());
    REQUIRE(spectrum.process_frame(extreme_bins));
    REQUIRE(std::isfinite(spectrum.current().magnitude_db[0]));
    REQUIRE(std::isfinite(spectrum.current().peak_db[0]));

    ScopeCaptureT<float, 8> scope;
    const auto scope_before = scope.config();
    auto scope_invalid = scope_before;
    scope_invalid.trigger_level = std::numeric_limits<double>::max();
    REQUIRE_FALSE(scope.configure(scope_invalid));
    scope_invalid.trigger_level = std::numeric_limits<float>::max();
    scope_invalid.hysteresis = std::numeric_limits<float>::max();
    REQUIRE_FALSE(scope.configure(scope_invalid));
    REQUIRE(scope.config().trigger_level == scope_before.trigger_level);
}

TEST_CASE("Analysis front ends allocate no memory while processing",
          "[signal][analysis-frontends][rt-safety]") {
    SpectrumTraceT<float, 513, 64> spectrum;
    SpectrumTraceConfig spectrum_config;
    spectrum_config.band_count = 32;
    REQUIRE(spectrum.configure(spectrum_config));
    std::array<float, 513> bins{};

    ScopeCaptureT<float, 1024> scope;
    ScopeCaptureConfig scope_config;
    scope_config.capture_samples = 512;
    scope_config.pretrigger_samples = 128;
    scope_config.trigger_mode = ScopeTriggerMode::immediate;
    scope_config.continuous = true;
    REQUIRE(scope.configure(scope_config));
    std::array<float, 257> samples{};

    bool processed = true;
    std::size_t allocation_count = 0;
    {
        pulp::test::RtAllocationProbe probe;
        for (int iteration = 0; iteration < 100; ++iteration) {
            processed = spectrum.process_frame(bins) && processed;
            scope.process(samples);
        }
        spectrum.reset();
        scope.reset();
        allocation_count = probe.allocation_count();
    }
    REQUIRE(processed);
    REQUIRE(allocation_count == 0);
}
