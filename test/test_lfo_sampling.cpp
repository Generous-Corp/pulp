// The three ways a consumer samples an LFO waveform, and the one stateful
// oscillator built on them. Every claim here is measured: the interval mean
// against a numeric integral, the band-limited waveform against the alias
// analyzer that grades every other bandlimited generator, and the block mean
// against the sample-by-sample oscillator it summarizes.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/audio/analysis/audio_spectrum.hpp>
#include <pulp/audio/buffer.hpp>
#include <pulp/signal/lfo.hpp>
#include <pulp/signal/lfo_sampling.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <utility>
#include <vector>

using Catch::Matchers::WithinAbs;
using pulp::signal::LfoShapeParams;
using pulp::signal::LfoWave;
using pulp::signal::lfo_wave;
using pulp::signal::lfo_wave_bandlimited;
using pulp::signal::lfo_wave_integral;
using pulp::signal::lfo_wave_is_periodic;
using pulp::signal::lfo_wave_mean;
using pulp::test::audio::AliasOptions;
using pulp::test::audio::AliasReport;
using pulp::test::audio::measure_aliasing;

namespace {

constexpr double kSampleRate = 48000.0;
constexpr int kFitLength = 8192;
constexpr double kBandHz = 20000.0;

constexpr LfoWave kPeriodic[] = {LfoWave::sine, LfoWave::triangle, LfoWave::saw_up,
                                 LfoWave::saw_down, LfoWave::square};

const LfoShapeParams kShapes[] = {{0.5, 0.5}, {0.3, 0.5}, {0.5, 0.2}, {0.75, 0.8}};

// Midpoint rule over a fine grid: the reference the closed form is held to.
double numeric_mean(LfoWave wave, double from, double to, const LfoShapeParams& params) {
    constexpr int kPoints = 200000;
    double acc = 0.0;
    for (int i = 0; i < kPoints; ++i)
        acc += lfo_wave(wave, from + (to - from) * (i + 0.5) / kPoints, params);
    return acc / kPoints;
}

AliasReport analyze(const std::vector<double>& signal, double f0) {
    pulp::audio::Buffer<float> buffer(1, static_cast<int>(signal.size()));
    for (int i = 0; i < static_cast<int>(signal.size()); ++i)
        buffer.channel(0)[i] = static_cast<float>(signal[static_cast<std::size_t>(i)]);
    AliasOptions options;
    options.num_harmonics = static_cast<int>(std::ceil(3.0 * kSampleRate / f0));
    options.analysis_length = static_cast<int>(signal.size());
    options.max_alias_frequency_hz = kBandHz;
    return measure_aliasing(std::as_const(buffer).view(), f0, kSampleRate, options);
}

// The waveform at `f0`, sampled every sample, trivially or band-limited.
std::vector<double> render(LfoWave wave, double f0, bool bandlimited,
                           const LfoShapeParams& params = {}) {
    const double increment = f0 / kSampleRate;
    std::vector<double> out(static_cast<std::size_t>(kFitLength));
    double phase = 0.137;
    for (auto& sample : out) {
        sample = bandlimited ? lfo_wave_bandlimited(wave, phase, increment, params)
                             : lfo_wave(wave, phase, params);
        phase += increment;
        phase -= std::floor(phase);
    }
    return out;
}

double variance(const std::vector<double>& x) {
    double mean = 0.0;
    for (double v : x) mean += v;
    mean /= static_cast<double>(x.size());
    double var = 0.0;
    for (double v : x) var += (v - mean) * (v - mean);
    return var / static_cast<double>(x.size());
}

}  // namespace

TEST_CASE("the interval mean of every periodic wave matches a numeric integral",
          "[signal][lfo][sampling]") {
    const std::pair<double, double> intervals[] = {
        {0.0, 1.0}, {0.1, 0.1 + 1.0 / 187.5 * 2048.0 / 48000.0}, {0.37, 0.41},
        {0.45, 0.55}, {0.9, 1.3}, {2.25, 5.75}, {-0.3, 0.2}, {0.49, 0.51}};
    for (LfoWave wave : kPeriodic)
        for (const auto& params : kShapes)
            for (const auto& [from, to] : intervals) {
                INFO("wave " << static_cast<int>(wave) << " pw " << params.pulse_width
                             << " peak " << params.triangle_peak << " [" << from << ", "
                             << to << "]");
                CHECK_THAT(lfo_wave_mean(wave, from, to, params),
                           WithinAbs(numeric_mean(wave, from, to, params), 2.0e-4));
                // The interval the other way round is the same interval.
                CHECK(lfo_wave_mean(wave, to, from, params)
                      == lfo_wave_mean(wave, from, to, params));
            }
}

TEST_CASE("the cumulative integral is continuous and a vanishing interval reads the point value",
          "[signal][lfo][sampling]") {
    for (LfoWave wave : kPeriodic)
        for (const auto& params : kShapes) {
            for (double phase : {0.0, 0.25, 0.3, 0.5, 0.8, 1.0, 2.5, -1.25}) {
                INFO("wave " << static_cast<int>(wave) << " phase " << phase);
                // Continuity across a cycle boundary and at the corners.
                CHECK_THAT(lfo_wave_integral(wave, phase + 1.0e-9, params),
                           WithinAbs(lfo_wave_integral(wave, phase - 1.0e-9, params), 1.0e-8));
                CHECK_THAT(lfo_wave_mean(wave, phase, phase, params),
                           WithinAbs(lfo_wave(wave, phase, params), 0.0));
                // Shrinking the interval converges on the point value (away
                // from a corner, where the point value is one-sided).
                const double p = phase + 0.013;
                CHECK_THAT(lfo_wave_mean(wave, p, p + 1.0e-6, params),
                           WithinAbs(lfo_wave(wave, p, params), 1.0e-4));
            }
            // A whole number of cycles averages to the wave's DC.
            const double dc = wave == LfoWave::square ? 2.0 * params.pulse_width - 1.0 : 0.0;
            CHECK_THAT(lfo_wave_mean(wave, 0.2, 3.2, params), WithinAbs(dc, 1.0e-12));
        }
    for (LfoWave wave : {LfoWave::sample_hold, LfoWave::smooth_random}) {
        CHECK_FALSE(lfo_wave_is_periodic(wave));
        CHECK(lfo_wave(wave, 0.3) == 0.0);
        CHECK(lfo_wave_mean(wave, 0.0, 1.0) == 0.0);
        CHECK(lfo_wave_bandlimited(wave, 0.3, 0.01) == 0.0);
    }
}

TEST_CASE("at a sub-audio rate the band-limited wave is the trivial one except on the two samples at each corner",
          "[signal][lfo][sampling]") {
    const double increment = 0.5 / kSampleRate;  // 0.5 Hz: 96 000 samples a cycle
    const std::size_t samples = 2 * 96000;
    for (LfoWave wave : {LfoWave::triangle, LfoWave::saw_up, LfoWave::saw_down, LfoWave::square}) {
        int differing = 0;
        double phase = 0.0;
        for (std::size_t i = 0; i < samples; ++i) {
            const double trivial = lfo_wave(wave, phase);
            const double bl = lfo_wave_bandlimited(wave, phase, increment);
            if (std::fabs(bl - trivial) > 1.0e-12) ++differing;
            phase += increment;
        }
        INFO("wave " << static_cast<int>(wave));
        // Two cycles; one corner per cycle for a saw, two for a square or a
        // triangle; at most two samples touched per corner.
        const int corners = (wave == LfoWave::saw_up || wave == LfoWave::saw_down) ? 2 : 4;
        CHECK(differing <= 2 * corners);
        CHECK(differing > 0);
    }
    // A stopped LFO has no corner to correct.
    CHECK(lfo_wave_bandlimited(LfoWave::square, 0.0, 0.0) == lfo_wave(LfoWave::square, 0.0));
}

TEST_CASE("at audio rate the band-limited waves alias less than the trivial ones",
          "[signal][lfo][sampling]") {
    constexpr double kF0[] = {1103.0, 2153.0, 4100.0};
    struct Case { LfoWave wave; double min_improvement_db; };
    const Case cases[] = {{LfoWave::saw_up, 8.0}, {LfoWave::saw_down, 8.0},
                          {LfoWave::square, 8.0}, {LfoWave::triangle, 3.0}};
    for (const auto& c : cases)
        for (double f0 : kF0) {
            const AliasReport trivial = analyze(render(c.wave, f0, false), f0);
            const AliasReport corrected = analyze(render(c.wave, f0, true), f0);
            INFO("wave " << static_cast<int>(c.wave) << " f0 " << f0 << " trivial "
                         << trivial.worst_alias_db << " dB, band-limited "
                         << corrected.worst_alias_db << " dB");
            REQUIRE_FALSE(trivial.has_unresolved_in_band_alias);
            REQUIRE_FALSE(corrected.has_unresolved_in_band_alias);
            CHECK(trivial.worst_alias_db - corrected.worst_alias_db >= c.min_improvement_db);
        }
}

TEST_CASE("block means of an LFO above the block Nyquist collapse to its average instead of aliasing",
          "[signal][lfo][sampling]") {
    // A 2048 Hz square read once per 256-sample block at 48 kHz (187.5 blocks
    // a second). The point sample is a stepper with the full swing; the
    // interval mean is the wave's average, flat to the box's precision.
    const double cycles_per_block = 2048.0 * 256.0 / kSampleRate;
    std::vector<double> points, means;
    double phase = 0.0;
    for (int block = 0; block < 400; ++block) {
        points.push_back(lfo_wave(LfoWave::square, phase));
        means.push_back(lfo_wave_mean(LfoWave::square, phase, phase + cycles_per_block));
        phase += cycles_per_block;
    }
    CHECK(variance(points) > 0.5);
    CHECK(variance(means) < 1.0e-3);

    // Well below the block Nyquist (10 Hz, 18.75 blocks a cycle) the mean is
    // the value at the block's centre to within the box's rolloff.
    const double slow = 10.0 * 256.0 / kSampleRate;
    phase = 0.0;
    double worst = 0.0;
    for (int block = 0; block < 400; ++block) {
        const double centre = lfo_wave(LfoWave::sine, phase + 0.5 * slow);
        worst = std::max(worst, std::fabs(lfo_wave_mean(LfoWave::sine, phase, phase + slow) - centre));
        phase += slow;
    }
    CHECK(worst < 0.02);
}

TEST_CASE("LfoT reads bit for bit as before unless band limiting is switched on",
          "[signal][lfo][sampling]") {
    using Lfo = pulp::signal::LfoT<float>;
    for (auto wave : {Lfo::Wave::triangle, Lfo::Wave::saw_up, Lfo::Wave::square}) {
        Lfo plain, limited;
        for (Lfo* l : {&plain, &limited}) {
            l->prepare(kSampleRate);
            l->set_wave(wave);
            l->set_rate_hz(1.0);
        }
        limited.set_band_limited(true);
        CHECK_FALSE(plain.band_limited());
        CHECK(limited.band_limited());
        int differing = 0;
        for (int i = 0; i < 96000; ++i) {
            const float a = plain.next(), b = limited.next();
            if (std::fabs(a - b) > 1.0e-6f) ++differing;
        }
        INFO("wave " << static_cast<int>(wave));
        // Two cycles at 1 Hz: at most two samples per corner differ.
        CHECK(differing <= 8);
        CHECK(differing > 0);
    }
}

TEST_CASE("LfoT::next_mean is the mean of next() over the block and lands on the same phase",
          "[signal][lfo][sampling]") {
    using Lfo = pulp::signal::LfoT<float>;
    for (auto wave : {Lfo::Wave::sine, Lfo::Wave::triangle, Lfo::Wave::saw_down, Lfo::Wave::square}) {
        Lfo stepped, summarized;
        for (Lfo* l : {&stepped, &summarized}) {
            l->prepare(kSampleRate);
            l->set_wave(wave);
            l->set_rate_hz(3.7);
            l->set_pulse_width(0.35f);
            l->set_tri_bias(0.4f);
        }
        for (int block = 0; block < 60; ++block) {
            double acc = 0.0;
            for (int i = 0; i < 256; ++i) acc += stepped.next();
            const float mean = summarized.next_mean(256);
            INFO("wave " << static_cast<int>(wave) << " block " << block);
            // A corner inside the block moves the discrete average by up to one
            // sample's share of the block; the curved waves agree far closer.
            const double tolerance = wave == Lfo::Wave::sine ? 1.0e-5 : 2.0 / 256.0;
            CHECK_THAT(static_cast<double>(mean), WithinAbs(acc / 256.0, tolerance));
            CHECK_THAT(summarized.phase(), WithinAbs(stepped.phase(), 1.0e-9));
        }
        CHECK(summarized.cycles_completed() == stepped.cycles_completed());
    }

    // A fading LFO averages next() itself, so the lifecycle reads the same;
    // once it sustains the closed form takes over, within the midpoint rule.
    Lfo stepped, summarized;
    for (Lfo* l : {&stepped, &summarized}) {
        l->prepare(kSampleRate);
        l->set_wave(Lfo::Wave::sine);
        l->set_rate_hz(5.0);
        l->set_fade_in_ms(100.0);
        l->retrigger();
    }
    for (int block = 0; block < 40; ++block) {
        double acc = 0.0;
        for (int i = 0; i < 256; ++i) acc += stepped.next();
        CHECK_THAT(static_cast<double>(summarized.next_mean(256)), WithinAbs(acc / 256.0, 1.0e-6));
    }

    // Above the block Nyquist the block means are flat where the point
    // samples swing.
    Lfo fast;
    fast.prepare(kSampleRate);
    fast.set_wave(Lfo::Wave::square);
    fast.set_rate_hz(2048.0);
    std::vector<double> means;
    for (int block = 0; block < 200; ++block) means.push_back(fast.next_mean(256));
    CHECK(variance(means) < 1.0e-3);
}
