// Tests for the lo-fi degradation stages: bit quantisation, sample-and-hold
// rate reduction with clock jitter, and the dead-zone output saturator.
//
// Each stage is checked against the property that makes it worth having rather
// than against its implementation. The quantiser is checked for its step
// count, the reducer for the hold interval it actually produces and for the
// spectral images that hold creates, the dead zone for the silence it imposes
// on small signals, and the whole chain for stage ordering and determinism.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <pulp/signal/lofi_chain.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <set>
#include <vector>

namespace {

using pulp::signal::dead_zone_saturate;
using pulp::signal::LofiChain64;
using pulp::signal::quantize_bits;
using pulp::signal::SampleRateReducer64;

constexpr double kPi = 3.14159265358979323846;

// Hann-windowed Goertzel amplitude at one frequency. The divisor includes the
// Hann window's coherent gain of 0.5 as well as the usual factor of two for a
// single-sided spectrum, so the return value is the tone's actual amplitude and
// the thresholds below can be read directly.
double goertzel(const std::vector<double>& x, double fs, double f) {
    const std::size_t n = x.size();
    const double w = 2.0 * kPi * f / fs;
    const double cw = std::cos(w);
    const double coeff = 2.0 * cw;
    double s1 = 0.0, s2 = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double win = 0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(i) /
                                                static_cast<double>(n - 1));
        const double s0 = coeff * s1 - s2 + win * x[i];
        s2 = s1;
        s1 = s0;
    }
    const double re = s1 - s2 * cw;
    const double im = s2 * std::sin(w);
    return std::sqrt(re * re + im * im) / (static_cast<double>(n) * 0.25);
}

std::vector<double> sine(double fs, double f, std::size_t n, double amplitude = 1.0) {
    std::vector<double> y(n);
    for (std::size_t i = 0; i < n; ++i) {
        y[i] = amplitude * std::sin(2.0 * kPi * f * static_cast<double>(i) / fs);
    }
    return y;
}

}  // namespace

TEST_CASE("Quantisation produces the number of levels its bit depth implies",
          "[signal][lofi]") {
    // Four bits, mid-tread and symmetric, gives 2^3 steps either side of zero,
    // so a full-scale sweep visits at most 17 distinct values.
    std::set<double> distinct;
    for (int i = -1000; i <= 1000; ++i) {
        distinct.insert(quantize_bits(i / 1000.0, 4.0));
    }
    REQUIRE(distinct.size() == 17);

    // The quantiser must not change the signal's scale.
    REQUIRE(quantize_bits(1.0, 4.0) == 1.0);
    REQUIRE(quantize_bits(-1.0, 4.0) == -1.0);
    REQUIRE(quantize_bits(0.0, 4.0) == 0.0);
}

TEST_CASE("Quantisation above 24 bits is transparent", "[signal][lofi]") {
    for (int i = -100; i <= 100; ++i) {
        const double x = i / 137.0;
        REQUIRE(quantize_bits(x, 24.0) == x);
        REQUIRE(quantize_bits(x, 32.0) == x);
    }
}

TEST_CASE("Quantisation error is bounded by half a step", "[signal][lofi]") {
    constexpr double kBits = 6.0;
    const double step = 1.0 / std::pow(2.0, kBits - 1.0);
    for (int i = -5000; i <= 5000; ++i) {
        const double x = i / 5000.0;
        REQUIRE(std::fabs(quantize_bits(x, kBits) - x) <= step * 0.5 + 1e-12);
    }
}

TEST_CASE("The dead zone silences small signals and passes large ones",
          "[signal][lofi]") {
    constexpr double kDz = 0.2;
    REQUIRE(dead_zone_saturate(0.0, kDz) == 0.0);
    REQUIRE(dead_zone_saturate(0.19, kDz) == 0.0);
    REQUIRE(dead_zone_saturate(-0.19, kDz) == 0.0);
    REQUIRE(dead_zone_saturate(0.5, kDz) > 0.0);
    REQUIRE(dead_zone_saturate(-0.5, kDz) < 0.0);
}

TEST_CASE("The dead-zone curve is continuous, odd, and bounded",
          "[signal][lofi]") {
    constexpr double kDz = 0.25;
    // Continuity at the edge of the gap: just above the threshold the output
    // must leave zero smoothly rather than jump.
    REQUIRE(std::fabs(dead_zone_saturate(kDz + 1e-6, kDz)) < 1e-5);

    double previous = dead_zone_saturate(-1.0, kDz);
    for (int i = -999; i <= 1000; ++i) {
        const double x = i / 1000.0;
        const double y = dead_zone_saturate(x, kDz);
        REQUIRE(std::fabs(y) <= 1.0);
        REQUIRE(y >= previous - 1e-12);  // monotone non-decreasing
        REQUIRE(std::fabs(dead_zone_saturate(-x, kDz) + y) < 1e-12);  // odd
        previous = y;
    }
}

TEST_CASE("A zero dead zone leaves the sign and shape intact",
          "[signal][lofi]") {
    for (int i = -100; i <= 100; ++i) {
        const double x = i / 100.0;
        const double y = dead_zone_saturate(x, 0.0);
        REQUIRE(std::fabs(y - std::tanh(1.5 * x)) < 1e-12);
    }
}

TEST_CASE("The rate reducer holds each sample for the requested interval",
          "[signal][lofi]") {
    constexpr double kFs = 48000.0;
    SampleRateReducer64 reducer;
    reducer.set_sample_rate(kFs);
    reducer.set_hold_rate_hz(6000.0);  // one new value every eight samples
    reducer.set_smoothing(0.0);
    reducer.reset();

    // Feed a ramp so every held value identifies the sample it was latched
    // from, then count how long each value survives.
    std::vector<double> out;
    out.reserve(400);
    for (int i = 0; i < 400; ++i) out.push_back(reducer.process(i / 400.0));

    int runs = 0;
    std::size_t run_start = 0;
    std::vector<int> run_lengths;
    for (std::size_t i = 1; i < out.size(); ++i) {
        if (out[i] != out[i - 1]) {
            run_lengths.push_back(static_cast<int>(i - run_start));
            run_start = i;
            ++runs;
        }
    }
    REQUIRE(runs > 40);
    for (int length : run_lengths) REQUIRE(length == 8);
}

TEST_CASE("Rate reduction creates images around the hold rate",
          "[signal][lofi]") {
    // A zero-order hold mirrors the input about the hold rate. That image is
    // the whole reason to use the stage, so its presence is the assertion.
    constexpr double kFs = 48000.0;
    constexpr double kHold = 6000.0;
    constexpr double kTone = 900.0;

    SampleRateReducer64 reducer;
    reducer.set_sample_rate(kFs);
    reducer.set_hold_rate_hz(kHold);
    reducer.set_smoothing(0.0);
    reducer.reset();

    const auto input = sine(kFs, kTone, 16384);
    std::vector<double> output(input.size());
    for (std::size_t i = 0; i < input.size(); ++i) output[i] = reducer.process(input[i]);

    const double fundamental = goertzel(output, kFs, kTone);
    const double lower_image = goertzel(output, kFs, kHold - kTone);
    const double upper_image = goertzel(output, kFs, kHold + kTone);

    REQUIRE(fundamental > 0.5);
    REQUIRE(lower_image > fundamental * 0.05);
    REQUIRE(upper_image > fundamental * 0.05);
}

TEST_CASE("Smoothing attenuates the images more than the signal",
          "[signal][lofi]") {
    // The reconstruction filter is one pole, so it thins the images out rather
    // than removing them. The property worth asserting is therefore selective:
    // the image-to-fundamental ratio must improve, and the fundamental itself
    // must survive -- a filter that simply turned everything down would satisfy
    // an image-level check alone.
    constexpr double kFs = 48000.0;
    constexpr double kHold = 6000.0;
    constexpr double kTone = 900.0;

    struct Measurement {
        double fundamental;
        double image;
    };

    auto measure = [](double smoothing) {
        SampleRateReducer64 reducer;
        reducer.set_sample_rate(kFs);
        reducer.set_hold_rate_hz(kHold);
        reducer.set_smoothing(smoothing);
        reducer.reset();
        const auto input = sine(kFs, kTone, 16384);
        std::vector<double> output(input.size());
        for (std::size_t i = 0; i < input.size(); ++i) {
            output[i] = reducer.process(input[i]);
        }
        return Measurement{goertzel(output, kFs, kTone),
                           goertzel(output, kFs, kHold + kTone)};
    };

    const auto raw = measure(0.0);
    const auto smoothed = measure(1.0);

    REQUIRE(smoothed.image < raw.image * 0.6);
    REQUIRE(smoothed.fundamental > raw.fundamental * 0.9);
    REQUIRE(smoothed.image / smoothed.fundamental < raw.image / raw.fundamental * 0.6);
}

TEST_CASE("A hold rate at or above the sample rate is transparent",
          "[signal][lofi]") {
    constexpr double kFs = 48000.0;
    SampleRateReducer64 reducer;
    reducer.set_sample_rate(kFs);
    reducer.set_hold_rate_hz(kFs);
    reducer.set_smoothing(0.0);
    reducer.reset();

    const auto input = sine(kFs, 1000.0, 512);
    for (std::size_t i = 0; i < input.size(); ++i) {
        REQUIRE(reducer.process(input[i]) == input[i]);
    }
}

TEST_CASE("Clock jitter perturbs the hold boundaries deterministically",
          "[signal][lofi]") {
    constexpr double kFs = 48000.0;

    auto run_lengths = [](double jitter, std::uint32_t seed) {
        SampleRateReducer64 reducer;
        reducer.set_sample_rate(kFs);
        reducer.set_hold_rate_hz(6000.0);
        reducer.set_smoothing(0.0);
        reducer.set_jitter(jitter);
        reducer.set_seed(seed);
        reducer.reset();

        std::vector<double> out;
        for (int i = 0; i < 800; ++i) out.push_back(reducer.process(i / 800.0));
        std::vector<int> lengths;
        std::size_t start = 0;
        for (std::size_t i = 1; i < out.size(); ++i) {
            if (out[i] != out[i - 1]) {
                lengths.push_back(static_cast<int>(i - start));
                start = i;
            }
        }
        return lengths;
    };

    const auto jittered = run_lengths(0.6, 12345u);
    const bool varies = std::any_of(jittered.begin(), jittered.end(),
                                    [](int n) { return n != 8; });
    REQUIRE(varies);

    // Same seed, same perturbation: a render must stay reproducible.
    REQUIRE(run_lengths(0.6, 12345u) == jittered);
    // Different seed, different perturbation.
    REQUIRE_FALSE(run_lengths(0.6, 999u) == jittered);
}

TEST_CASE("A default chain is a pass-through", "[signal][lofi]") {
    // Every stage defaults to transparent, so a caller that wants one stage
    // does not silently get the other two.
    LofiChain64 chain;
    chain.set_sample_rate(48000.0);
    chain.reset();

    const auto input = sine(48000.0, 440.0, 256);
    for (std::size_t i = 0; i < input.size(); ++i) {
        REQUIRE(chain.process(input[i]) == input[i]);
    }
}

TEST_CASE("The chain quantises before it holds", "[signal][lofi]") {
    // Stage order is a property of the class, not of each caller. Quantising
    // first means every held value is already on the quantisation grid; the
    // reverse order would let the hold latch an unquantised sample.
    constexpr double kFs = 48000.0;
    constexpr double kBits = 4.0;
    const double step = 1.0 / std::pow(2.0, kBits - 1.0);

    LofiChain64 chain;
    chain.set_sample_rate(kFs);
    chain.set_bits(kBits);
    chain.set_hold_rate_hz(4000.0);
    chain.set_smoothing(0.0);
    chain.reset();

    const auto input = sine(kFs, 300.0, 2048);
    for (std::size_t i = 0; i < input.size(); ++i) {
        const double y = chain.process(input[i]);
        const double on_grid = std::round(y / step) * step;
        REQUIRE(std::fabs(y - on_grid) < 1e-12);
    }
}

TEST_CASE("The chain's dead zone silences a decaying tail",
          "[signal][lofi]") {
    // The audible signature of the dead zone is that a fade does not fade --
    // it thins out and stops. Drive a decaying sine through and check the
    // output has gone to exact zero while the input is still non-zero.
    constexpr double kFs = 48000.0;
    LofiChain64 chain;
    chain.set_sample_rate(kFs);
    chain.set_dead_zone(0.3);
    chain.reset();

    double last_nonzero_input = 0.0;
    bool output_silent = false;
    for (int i = 0; i < 48000; ++i) {
        const double envelope = std::exp(-static_cast<double>(i) / 4000.0);
        const double x = envelope * std::sin(2.0 * kPi * 200.0 * i / kFs);
        const double y = chain.process(x);
        if (i > 20000) {
            if (y == 0.0 && std::fabs(x) > 1e-6) {
                output_silent = true;
                last_nonzero_input = std::fabs(x);
            }
        }
    }
    REQUIRE(output_silent);
    REQUIRE(last_nonzero_input > 0.0);
}

TEST_CASE("The lo-fi chain allocates nothing on the audio thread",
          "[signal][lofi][rt-safety]") {
    LofiChain64 chain;
    chain.set_sample_rate(48000.0);
    chain.set_bits(8.0);
    chain.set_hold_rate_hz(11025.0);
    chain.set_jitter(0.4);
    chain.set_smoothing(0.6);
    chain.set_dead_zone(0.05);
    chain.reset();

    std::vector<double> buffer(4096);
    for (std::size_t i = 0; i < buffer.size(); ++i) {
        buffer[i] = std::sin(2.0 * kPi * 220.0 * static_cast<double>(i) / 48000.0);
    }

    double sink = 0.0;
    std::size_t allocations = 0;
    {
        pulp::test::RtAllocationProbe probe;
        for (int block = 0; block < 4; ++block) {
            chain.process(buffer.data(), static_cast<int>(buffer.size()));
            for (double v : buffer) sink += v;
        }
        chain.reset();
        allocations = probe.allocation_count();
    }

    REQUIRE(std::isfinite(sink));
    REQUIRE(allocations == 0);
}

// The floor shaper is checked for the geometry that distinguishes it from the
// dead-zone saturator — silence below the threshold, full scale restored above
// it — and for the property that makes the curve choice worth offering: only
// smoothstep leaves the threshold with zero slope. The last case pins the
// arithmetic a caller sees when it supplies its own dry/wet mix, because that
// mix deliberately lives outside this function.

TEST_CASE("The floor shaper silences everything under its threshold",
          "[signal][lofi]") {
    using pulp::signal::floor_shape;
    using pulp::signal::FloorCurve;

    for (const auto curve : {FloorCurve::linear, FloorCurve::smoothstep}) {
        REQUIRE(floor_shape(0.05, 0.1, curve) == 0.0);
        REQUIRE(floor_shape(-0.05, 0.1, curve) == 0.0);
        REQUIRE(floor_shape(0.1, 0.1, curve) == 0.0);
        REQUIRE(floor_shape(0.0, 0.1, curve) == 0.0);
    }
}

TEST_CASE("The floor shaper rescales the surviving region to full scale",
          "[signal][lofi]") {
    using pulp::signal::floor_shape;
    using pulp::signal::FloorCurve;

    // Just above the threshold the output leaves silence, and at full scale it
    // arrives at full scale, for either curve.
    REQUIRE(floor_shape(0.1 + 1e-9, 0.1, FloorCurve::linear) > 0.0);
    REQUIRE(floor_shape(1.0, 0.1, FloorCurve::linear) == Catch::Approx(1.0));
    REQUIRE(floor_shape(1.0, 0.1, FloorCurve::smoothstep) == Catch::Approx(1.0));

    // A half-way input maps to the midpoint of the rescaled range under the
    // linear curve.
    REQUIRE(floor_shape(0.55, 0.1, FloorCurve::linear) == Catch::Approx(0.5));
}

TEST_CASE("The floor shaper preserves sign", "[signal][lofi]") {
    using pulp::signal::floor_shape;
    using pulp::signal::FloorCurve;

    for (const auto curve : {FloorCurve::linear, FloorCurve::smoothstep}) {
        const double positive = floor_shape(0.7, 0.2, curve);
        const double negative = floor_shape(-0.7, 0.2, curve);
        REQUIRE(positive > 0.0);
        REQUIRE(negative < 0.0);
        REQUIRE(negative == Catch::Approx(-positive));
    }
}

TEST_CASE("Only the smoothstep curve leaves the threshold with zero slope",
          "[signal][lofi]") {
    using pulp::signal::floor_shape;
    using pulp::signal::FloorCurve;

    // This is the whole reason the curve is selectable: measured just above the
    // threshold, the linear ramp already has the slope it will keep, while
    // smoothstep is still nearly flat.
    const double threshold = 0.1;
    const double delta = 1e-4;
    const double linear_rise =
        floor_shape(threshold + delta, threshold, FloorCurve::linear);
    const double smooth_rise =
        floor_shape(threshold + delta, threshold, FloorCurve::smoothstep);

    REQUIRE(smooth_rise < linear_rise);
    REQUIRE(smooth_rise < linear_rise * 0.01);
}

TEST_CASE("The floor shaper clamps its threshold into the usable range",
          "[signal][lofi]") {
    using pulp::signal::floor_shape;
    using pulp::signal::FloorCurve;

    // A negative threshold behaves as zero, so nothing is discarded.
    REQUIRE(floor_shape(0.5, -1.0, FloorCurve::linear) == Catch::Approx(0.5));

    // A threshold past the ceiling is held at 0.9, which keeps the rescale
    // divisor away from zero. Probing at full scale cannot show where the
    // ceiling is -- any ceiling in (0,1) maps 1.0 to 1.0 -- so probe inside the
    // surviving region instead, where 0.95 lands midway only if the ceiling is
    // exactly 0.9.
    REQUIRE(floor_shape(1.0, 5.0, FloorCurve::linear) == Catch::Approx(1.0));
    REQUIRE(floor_shape(0.95, 5.0, FloorCurve::linear) == Catch::Approx(0.5));
}

TEST_CASE("The smoothstep curve is the cubic it claims to be",
          "[signal][lofi]") {
    using pulp::signal::floor_shape;
    using pulp::signal::FloorCurve;

    // Pinning only the endpoints and the initial slope admits other cubics.
    // n*n*(2-n) also starts flat and reaches full scale, so these two interior
    // points -- one at the symmetric midpoint, one off-centre -- are what
    // distinguish 3n^2-2n^3 from its neighbours.
    REQUIRE(floor_shape(0.55, 0.1, FloorCurve::smoothstep) ==
            Catch::Approx(0.5));
    REQUIRE(floor_shape(0.325, 0.1, FloorCurve::smoothstep) ==
            Catch::Approx(0.15625));

    // The same quarter-point under the linear curve, so the two are not
    // silently interchangeable.
    REQUIRE(floor_shape(0.325, 0.1, FloorCurve::linear) == Catch::Approx(0.25));
}

TEST_CASE("Input beyond full scale saturates instead of turning over",
          "[signal][lofi]") {
    using pulp::signal::floor_shape;
    using pulp::signal::FloorCurve;

    // Without the clamp the smoothstep polynomial leaves its valid interval:
    // it falls back to zero at a normalised 1.5 and then goes negative, which
    // copysign would return as a large positive sample. An EQ in front of this
    // makes that reachable, so both curves are held at full scale instead.
    for (const auto curve : {FloorCurve::linear, FloorCurve::smoothstep}) {
        REQUIRE(floor_shape(1.45, 0.1, curve) == Catch::Approx(1.0));
        REQUIRE(floor_shape(2.0, 0.1, curve) == Catch::Approx(1.0));
        REQUIRE(floor_shape(-2.0, 0.1, curve) == Catch::Approx(-1.0));
        REQUIRE(floor_shape(1e6, 0.1, curve) == Catch::Approx(1.0));
    }
}

TEST_CASE("The floor shaper accepts a float threshold beside a float sample",
          "[signal][lofi]") {
    using pulp::signal::floor_shape;
    using pulp::signal::FloorCurve;

    // The threshold is a non-deduced parameter so a caller may mix a literal
    // with a typed sample. Deducing it would make this line a compile error,
    // which is exactly the shape a float call site reaches for.
    const float shaped = floor_shape(0.55f, 0.1, FloorCurve::linear);
    REQUIRE(shaped == Catch::Approx(0.5f));
    REQUIRE(floor_shape(0.55, 0.1f, FloorCurve::linear) == Catch::Approx(0.5));
}

TEST_CASE("Non-finite input propagates rather than being silently swallowed",
          "[signal][lofi]") {
    using pulp::signal::floor_shape;
    using pulp::signal::FloorCurve;

    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();

    // Infinity saturates, because the clamp bounds it like any other
    // over-range input.
    REQUIRE(floor_shape(inf, 0.1, FloorCurve::linear) == Catch::Approx(1.0));
    REQUIRE(floor_shape(-inf, 0.1, FloorCurve::linear) == Catch::Approx(-1.0));

    // NaN is not silenced into a plausible-looking zero. A caller that can
    // produce one is expected to guard upstream.
    REQUIRE(std::isnan(floor_shape(nan, 0.1, FloorCurve::linear)));
    REQUIRE(std::isnan(floor_shape(0.5, nan, FloorCurve::linear)));
}
