#include "harness/rt_allocation_probe.hpp"
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/signal/explicit_q_resonator_bank.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <limits>
#include <thread>
#include <utility>
#include <vector>

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using pulp::signal::ExplicitQResonatorBank;

namespace {
constexpr double kSampleRate = 48000.0;
constexpr double kPi = 3.14159265358979323846;

ExplicitQResonatorBank::BandRecipe band(double frequency, double q = 8.0, double gain_db = 0.0) {
    return {.frequency_hz = frequency,
            .q = q,
            .gain_db = gain_db,
            .attack_ms = 1.0,
            .release_ms = 20.0};
}

double render_rms(double input_frequency, double resonator_frequency, double q = 12.0) {
    ExplicitQResonatorBank bank;
    REQUIRE(bank.prepare(kSampleRate, 1));
    REQUIRE(bank.stage_band(0, band(resonator_frequency, q)));
    REQUIRE(bank.publish(1, 0.0));
    double sum = 0.0;
    for (int i = 0; i < 48000; ++i) {
        const float input =
            static_cast<float>(std::sin(2.0 * kPi * input_frequency * i / kSampleRate));
        const double output = bank.process(input);
        if (i >= 24000)
            sum += output * output;
    }
    return std::sqrt(sum / 24000.0);
}

double goertzel(const std::vector<float>& samples, double frequency) {
    const double radians = 2.0 * kPi * frequency / kSampleRate;
    const double cosine = std::cos(radians);
    const double coefficient = 2.0 * cosine;
    double previous = 0.0;
    double previous_two = 0.0;
    for (std::size_t i = 0; i < samples.size(); ++i) {
        const double window =
            0.5 - 0.5 * std::cos(2.0 * kPi * i / static_cast<double>(samples.size() - 1));
        const double current =
            coefficient * previous - previous_two + window * static_cast<double>(samples[i]);
        previous_two = previous;
        previous = current;
    }
    const double real = previous - previous_two * cosine;
    const double imaginary = previous_two * std::sin(radians);
    return std::hypot(real, imaginary);
}
} // namespace

TEST_CASE("ExplicitQResonatorBank rejects an invalid recipe atomically",
          "[signal][explicit-q][transaction]") {
    ExplicitQResonatorBank bank;
    REQUIRE(bank.prepare(kSampleRate, 2));
    REQUIRE(bank.stage_band(0, band(440.0)));
    REQUIRE(bank.publish(1, 0.0));
    (void)bank.process(0.0f);

    REQUIRE(bank.stage_band(0, band(880.0)));
    auto invalid = band(1200.0);
    invalid.q = std::numeric_limits<double>::quiet_NaN();
    REQUIRE(bank.stage_band(1, invalid));
    REQUIRE_FALSE(bank.publish(2));
    REQUIRE_FALSE(bank.publish(1, 500.001));
    REQUIRE_FALSE(bank.set_coefficient_transition_ms(std::numeric_limits<double>::quiet_NaN()));
    (void)bank.process(0.0f);
    REQUIRE(bank.active_band_count() == 1);
    REQUIRE_THAT(bank.applied_band_recipe(0).frequency_hz, WithinAbs(440.0, 0.0));
}

TEST_CASE("ExplicitQResonatorBank transitions land on the declared sample",
          "[signal][explicit-q][transition]") {
    for (double transition_ms : {0.0, 5.0, 20.0, 500.0}) {
        ExplicitQResonatorBank bank;
        REQUIRE(bank.prepare(kSampleRate, 1));
        REQUIRE(bank.stage_band(0, band(400.0, 2.0, -12.0)));
        REQUIRE(bank.publish(1, 0.0));
        (void)bank.process(0.0f);
        REQUIRE(bank.stage_band(0, band(1000.0, 10.0, 6.0)));
        if (transition_ms == 20.0) {
            REQUIRE(bank.set_coefficient_transition_ms(transition_ms));
            REQUIRE(bank.publish(1));
        } else {
            REQUIRE(bank.publish(1, transition_ms));
        }
        const int samples = static_cast<int>(std::ceil(transition_ms * kSampleRate / 1000.0));
        double previous = 400.0;
        bool monotonic_without_overshoot = true;
        for (int i = 0; i < std::max(samples - 1, 0); ++i) {
            (void)bank.process(0.0f);
            const double current = bank.applied_band_recipe(0).frequency_hz;
            monotonic_without_overshoot &= current >= previous && current <= 1000.0;
            previous = current;
        }
        REQUIRE(monotonic_without_overshoot);
        if (samples > 1)
            REQUIRE(bank.applied_band_recipe(0).frequency_hz < 1000.0);
        (void)bank.process(0.0f);
        REQUIRE_THAT(bank.applied_band_recipe(0).frequency_hz, WithinAbs(1000.0, 1e-12));
        REQUIRE_THAT(bank.applied_band_recipe(0).q, WithinAbs(10.0, 1e-12));
        REQUIRE_THAT(bank.applied_band_recipe(0).gain_db, WithinAbs(6.0, 1e-12));
    }
}

TEST_CASE("ExplicitQResonatorBank has a frequency-selective bandpass response",
          "[signal][explicit-q][audio]") {
    const double centered = render_rms(1000.0, 1000.0);
    const double off_band = render_rms(250.0, 1000.0);
    INFO("centered RMS=" << centered << " off-band RMS=" << off_band);
    REQUIRE(centered > off_band * 8.0);
}

TEST_CASE("ExplicitQResonatorBank center and Q agree with independent half-power points",
          "[signal][explicit-q][response]") {
    for (double q : {0.5, 2.0, 10.0, 100.0}) {
        constexpr double center_hz = 1000.0;
        const double warped_center = std::tan(kPi * center_hz / kSampleRate);
        const double damping = 1.0 / q;
        const double root = std::sqrt(damping * damping + 4.0);
        const double low_ratio = 0.5 * (root - damping);
        const double high_ratio = 0.5 * (root + damping);
        const double low_hz = kSampleRate / kPi * std::atan(warped_center * low_ratio);
        const double high_hz = kSampleRate / kPi * std::atan(warped_center * high_ratio);
        const double center = render_rms(center_hz, center_hz, q);
        const double low = render_rms(low_hz, center_hz, q);
        const double high = render_rms(high_hz, center_hz, q);
        INFO("q=" << q << " low=" << low_hz << " high=" << high_hz);
        REQUIRE_THAT(center / low, WithinAbs(std::sqrt(2.0), 0.025));
        REQUIRE_THAT(center / high, WithinAbs(std::sqrt(2.0), 0.025));
        REQUIRE_THAT(center_hz / (high_hz - low_hz), WithinAbs(q, 0.1 * q));
    }

    for (const auto [frequency, off_frequency] :
         {std::pair{20.0, 80.0}, std::pair{1000.0, 250.0}, std::pair{21600.0, 10800.0}}) {
        const double centered = render_rms(frequency, frequency, 2.0);
        const double off = render_rms(off_frequency, frequency, 2.0);
        INFO("frequency=" << frequency << " centered=" << centered << " off=" << off);
        REQUIRE(centered > off * 1.5);
    }
}

TEST_CASE("ExplicitQResonatorBank envelope follows the pre-gain band output",
          "[signal][explicit-q][ballistics]") {
    ExplicitQResonatorBank bank;
    REQUIRE(bank.prepare(kSampleRate, 1));
    REQUIRE(bank.stage_band(0, band(1000.0, 8.0, 18.0)));
    REQUIRE(bank.publish(1, 0.0));
    pulp::signal::BallisticsFilterT<double> oracle;
    oracle.prepare(kSampleRate);
    oracle.set_attack_ms(1.0);
    oracle.set_release_ms(20.0);
    for (int i = 0; i < 4096; ++i) {
        (void)bank.process(static_cast<float>(std::sin(2.0 * kPi * 1000.0 * i / kSampleRate)));
        const double expected = oracle.process(std::abs(bank.band_output(0)));
        REQUIRE_THAT(bank.envelope_at(0), WithinRel(expected, 1e-7));
    }
}

TEST_CASE("ExplicitQResonatorBank gain changes only the summed output",
          "[signal][explicit-q][gain]") {
    ExplicitQResonatorBank unity, boosted;
    REQUIRE(unity.prepare(kSampleRate, 1));
    REQUIRE(boosted.prepare(kSampleRate, 1));
    REQUIRE(unity.stage_band(0, band(1000.0, 8.0, 0.0)));
    REQUIRE(boosted.stage_band(0, band(1000.0, 8.0, 6.020599913279624)));
    REQUIRE(unity.publish(1, 0.0));
    REQUIRE(boosted.publish(1, 0.0));
    for (int i = 0; i < 4096; ++i) {
        const float input = static_cast<float>(std::sin(2.0 * kPi * 1000.0 * i / kSampleRate));
        const double a = unity.process(input);
        const double b = boosted.process(input);
        REQUIRE_THAT(boosted.band_output(0), WithinAbs(unity.band_output(0), 0.0));
        REQUIRE_THAT(boosted.envelope_at(0), WithinAbs(unity.envelope_at(0), 0.0));
        REQUIRE_THAT(b, WithinAbs(2.0 * a, 2e-7));
    }
}

TEST_CASE("ExplicitQResonatorBank turns deterministic noise into configured spectral peaks",
          "[signal][explicit-q][composition]") {
    constexpr std::array<double, 5> frequencies{180.0, 500.0, 1400.0, 4000.0, 11000.0};
    constexpr std::array<double, 5> gains_db{-6.0, -3.0, 0.0, 3.0, 6.0};
    ExplicitQResonatorBank bank;
    REQUIRE(bank.prepare(kSampleRate, static_cast<int>(frequencies.size())));
    for (std::size_t i = 0; i < frequencies.size(); ++i)
        REQUIRE(bank.stage_band(static_cast<int>(i), band(frequencies[i], 30.0, gains_db[i])));
    REQUIRE(bank.publish(static_cast<int>(frequencies.size()), 0.0));

    constexpr std::size_t kWarmup = 65536;
    constexpr std::size_t kMeasure = 131072;
    std::vector<float> measured_input(kMeasure), measured_output(kMeasure);
    std::uint32_t random = 0x5eed1234u;
    for (std::size_t i = 0; i < kWarmup + kMeasure; ++i) {
        random = random * 1664525u + 1013904223u;
        const float input = 0.2f * (static_cast<float>(random) / 4294967296.0f - 0.5f);
        const float output = bank.process(input);
        if (i >= kWarmup) {
            measured_input[i - kWarmup] = input;
            measured_output[i - kWarmup] = output;
        }
    }
    std::array<double, frequencies.size()> measured_gain_db{};
    for (std::size_t i = 0; i < frequencies.size(); ++i) {
        const auto transfer_db = [&](double frequency) {
            return 20.0 * std::log10(goertzel(measured_output, frequency) /
                                     (goertzel(measured_input, frequency) + 1e-30));
        };
        const double center = transfer_db(frequencies[i]);
        INFO("band=" << i << " frequency=" << frequencies[i] << " transfer=" << center);
        REQUIRE(center > transfer_db(frequencies[i] * 0.98) + 1.0);
        REQUIRE(center > transfer_db(frequencies[i] * 1.02) + 1.0);
        measured_gain_db[i] = center;
    }
    for (std::size_t i = 1; i < frequencies.size(); ++i)
        REQUIRE_THAT(measured_gain_db[i] - measured_gain_db[0],
                     WithinAbs(gains_db[i] - gains_db[0], 1.5));
}

TEST_CASE("ExplicitQResonatorBank is zero-latency and its recursive tail obeys the Q bound",
          "[signal][explicit-q][tail]") {
    ExplicitQResonatorBank bank;
    const auto recipe = band(1000.0, 10.0);
    REQUIRE(bank.prepare(kSampleRate, 1));
    REQUIRE(bank.stage_band(0, recipe));
    REQUIRE(bank.publish(1, 0.0));
    REQUIRE(std::abs(bank.process(1.0f)) > 0.0f);
    const int t60 = static_cast<int>(
        std::ceil(ExplicitQResonatorBank::estimated_t60_seconds(recipe) * kSampleRate));
    double peak_after_t60 = 0.0;
    for (int i = 1; i < t60 + 1024; ++i) {
        const double value = std::abs(bank.process(0.0f));
        if (i >= t60)
            peak_after_t60 = std::max(peak_after_t60, value);
    }
    REQUIRE(peak_after_t60 < 0.002);
    for (int i = 0; i < 100 * t60; ++i)
        (void)bank.process(0.0f);
    REQUIRE(bank.band_output(0) == 0.0);
    REQUIRE(bank.envelope_at(0) == 0.0);
    REQUIRE(bank.latency_samples() == 0);
}

TEST_CASE("ExplicitQResonatorBank block and sample processing are bit identical",
          "[signal][explicit-q][block]") {
    std::vector<float> input(2048), block_output(2048), sample_output(2048);
    for (std::size_t i = 0; i < input.size(); ++i)
        input[i] = static_cast<float>(std::sin(2.0 * kPi * 733.0 * i / kSampleRate));
    ExplicitQResonatorBank a, b;
    REQUIRE(a.prepare(kSampleRate, 1));
    REQUIRE(b.prepare(kSampleRate, 1));
    REQUIRE(a.stage_band(0, band(733.0)));
    REQUIRE(b.stage_band(0, band(733.0)));
    REQUIRE(a.publish(1, 0.0));
    REQUIRE(b.publish(1, 0.0));
    a.process(input.data(), block_output.data(), static_cast<int>(input.size()));
    for (std::size_t i = 0; i < input.size(); ++i)
        sample_output[i] = b.process(input[i]);
    REQUIRE(block_output == sample_output);
}

TEST_CASE("ExplicitQResonatorBank process is allocation-free and contains hostile input",
          "[signal][explicit-q][rt-safety]") {
    ExplicitQResonatorBank bank;
    REQUIRE(bank.prepare(kSampleRate, ExplicitQResonatorBank::kMaximumBands));
    for (int i = 0; i < ExplicitQResonatorBank::kMaximumBands; ++i)
        REQUIRE(bank.stage_band(i, band(100.0 + 300.0 * i, 2.0)));
    REQUIRE(bank.publish(ExplicitQResonatorBank::kMaximumBands, 20.0));
    std::vector<float> input(4096, 0.25f), output(4096);
    bank.process(input.data(), output.data(), static_cast<int>(input.size()));
    bool process_allocated = false;
    {
        pulp::test::RtAllocationProbe probe;
        bank.process(input.data(), output.data(), static_cast<int>(input.size()));
        process_allocated = probe.saw_allocation();
    }
    REQUIRE_FALSE(process_allocated);
    REQUIRE(std::isfinite(bank.process(std::numeric_limits<float>::max())));
    REQUIRE(std::isfinite(bank.band_output(0)));
    REQUIRE(std::isfinite(bank.envelope_at(0)));
    REQUIRE(bank.process(std::numeric_limits<float>::infinity()) == 0.0f);
    for (double value : {bank.band_output(0), bank.envelope_at(0)})
        REQUIRE(std::isfinite(value));
    REQUIRE(bank.latency_samples() == 0);
    REQUIRE(bank.retained_bytes() > 0);
    bool republished = false;
    bool publication_allocated = false;
    {
        pulp::test::RtAllocationProbe publication_probe;
        republished = bank.publish(ExplicitQResonatorBank::kMaximumBands, 20.0);
        publication_allocated = publication_probe.saw_allocation();
    }
    REQUIRE(republished);
    REQUIRE_FALSE(publication_allocated);
}

TEST_CASE("ExplicitQResonatorBank publishes complete recipes during concurrent processing",
          "[signal][explicit-q][concurrency]") {
    ExplicitQResonatorBank bank;
    REQUIRE(bank.prepare(kSampleRate, 2));
    std::atomic<bool> done{false};
    std::atomic<bool> publish_failed{false};
    std::thread writer([&] {
        for (int generation = 1; generation <= 10000; ++generation) {
            const double first = generation == 10000 ? 2000.0 : 200.0 + generation % 1800;
            if (!bank.stage_band(0, band(first, 2.0 + generation % 20, -12.0)) ||
                !bank.stage_band(1, band(3000.0, 4.0, 6.0)) || !bank.publish(2, 0.0))
                publish_failed.store(true, std::memory_order_relaxed);
        }
        done.store(true, std::memory_order_release);
    });
    bool saw_non_finite = false;
    while (!done.load(std::memory_order_acquire))
        saw_non_finite |= !std::isfinite(bank.process(0.125f));
    writer.join();
    (void)bank.process(0.0f);
    REQUIRE_FALSE(publish_failed.load(std::memory_order_relaxed));
    REQUIRE_FALSE(saw_non_finite);
    REQUIRE(bank.active_band_count() == 2);
    REQUIRE_THAT(bank.applied_band_recipe(0).frequency_hz, WithinAbs(2000.0, 0.0));
}

TEST_CASE("ExplicitQResonatorBank prepare is failure-atomic", "[signal][explicit-q]") {
    ExplicitQResonatorBank bank;
    REQUIRE(bank.prepare(kSampleRate, 2));
    const auto bytes = bank.retained_bytes();
    REQUIRE_FALSE(bank.prepare(std::numeric_limits<double>::quiet_NaN(), 2));
    REQUIRE_FALSE(bank.prepare(40.0, 2));
    REQUIRE(bank.capacity() == 2);
    REQUIRE(bank.retained_bytes() == bytes);
}

TEST_CASE("ExplicitQResonatorBank reset clears state and lands an active transition",
          "[signal][explicit-q][reset]") {
    ExplicitQResonatorBank bank;
    REQUIRE(bank.prepare(kSampleRate, 1));
    REQUIRE(bank.stage_band(0, band(400.0)));
    REQUIRE(bank.publish(1, 0.0));
    (void)bank.process(1.0f);
    REQUIRE(bank.stage_band(0, band(1200.0)));
    REQUIRE(bank.publish(1, 500.0));
    (void)bank.process(0.0f);
    REQUIRE(bank.applied_band_recipe(0).frequency_hz < 1200.0);
    bank.reset();
    REQUIRE(bank.band_output(0) == 0.0);
    REQUIRE(bank.envelope_at(0) == 0.0);
    REQUIRE_THAT(bank.applied_band_recipe(0).frequency_hz, WithinAbs(1200.0, 0.0));
}
