// Accuracy contracts of the branch-free math helpers when the compiler does
// not fuse multiply-adds. This file is compiled with -ffp-contract=off on
// GCC and Clang (MSVC does not contract by default), so every bound below is
// the bound a GCC, MSVC or x86-64-without-FMA build gets.
#include <catch2/catch_test_macros.hpp>
#include <pulp/signal/compressor.hpp>
#include <pulp/signal/fast_math.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

using namespace pulp::signal;

namespace {

double ulp_of(double value) {
    const float f = static_cast<float>(std::abs(value));
    return static_cast<double>(std::nextafter(f, std::numeric_limits<float>::infinity()) - f);
}

// Pins that this TU really does not contract: with fusion, a*b - a*b on
// these operands returns the rounding error of a*b instead of zero.
float product_difference(float a, float b) {
    volatile float va = a;
    volatile float vb = b;
    return va * vb - va * vb;
}

} // namespace

TEST_CASE("Branch-free math holds its bounds without fused multiply-add",
          "[signal][fast-math][no-contract]") {
    REQUIRE(product_difference(1.0f + 1.0f / 4096.0f, 1.0f - 1.0f / 8192.0f) == 0.0f);

    double log10_ulps = 0.0;
    for (float x = 1.0e-10f; x < 1.0e6f; x *= 1.0001f) {
        const double reference = std::log10(static_cast<double>(x));
        const double err = std::abs(static_cast<double>(FastMath::log10_accurate(x)) - reference);
        if (std::abs(reference) >= 1.0e-3)
            log10_ulps = std::max(log10_ulps, err / ulp_of(reference));
    }
    CHECK(log10_ulps <= 4.0);

    double exp10_rel = 0.0;
    for (float x = -37.0f; x < 38.0f; x += 1.0e-4f) {
        const double reference = std::pow(10.0, static_cast<double>(x));
        exp10_rel = std::max(
            exp10_rel,
            std::abs(static_cast<double>(FastMath::exp10_accurate(x)) - reference) / reference);
    }
    CHECK(exp10_rel < 2.0e-7);
    CHECK(std::isnan(FastMath::exp10_accurate(std::numeric_limits<float>::quiet_NaN())));

    double tanh_err = 0.0;
    for (std::uint32_t bits = 0; bits < 0x7f800000u; bits += 64) {
        float x;
        std::memcpy(&x, &bits, sizeof x);
        tanh_err = std::max(tanh_err, std::abs(static_cast<double>(FastMath::tanh_accurate(x)) -
                                               std::tanh(static_cast<double>(x))));
    }
    CHECK(tanh_err < 3.6e-7);
    for (float x = 9.0001f; x < 9.5f; x = std::nextafter(x, 10.0f)) {
        if (FastMath::tanh_accurate(x) != 1.0f) {
            FAIL("tanh_accurate(" << x << ") is not exactly 1");
        }
    }

    double pade_err = 0.0;
    double recursive_err = 0.0;
    float pade_max = 0.0f;
    float recursive_max = 0.0f;
    for (std::uint32_t bits = 0; bits < 0x7f800000u; bits += 64) {
        float x;
        std::memcpy(&x, &bits, sizeof x);
        const double reference = std::tanh(static_cast<double>(x));
        pade_err = std::max(pade_err, std::abs(static_cast<double>(FastMath::tanh(x)) - reference));
        recursive_err = std::max(
            recursive_err,
            std::abs(static_cast<double>(FastMath::tanh_minimax_recursive(x)) - reference));
        pade_max = std::max(pade_max, FastMath::tanh(x));
        recursive_max = std::max(recursive_max, FastMath::tanh_minimax_recursive(x));
    }
    CHECK(pade_err < 5.8e-5);
    CHECK(pade_max < 1.0f);
    CHECK(recursive_err < 3.8e-6);
    CHECK(recursive_max <= 1.0f);

    // tanh_minimax: its documented bound and |y| <= 1, every 64th float and
    // every float around the worst case (|x| near 4.745).
    double minimax_err = 0.0;
    float minimax_largest = 0.0f;
    auto visit = [&](float x) {
        const float y = FastMath::tanh_minimax(x);
        minimax_err = std::max(
            minimax_err, std::abs(static_cast<double>(y) - std::tanh(static_cast<double>(x))));
        minimax_largest = std::max(minimax_largest, std::abs(y));
    };
    for (std::uint32_t bits = 0; bits < 0x7f800000u; bits += 64) {
        float x;
        std::memcpy(&x, &bits, sizeof x);
        visit(x);
        visit(-x);
    }
    for (float x = 4.6f; x < 4.9f; x = std::nextafter(x, 5.0f)) {
        visit(x);
        visit(-x);
    }
    CHECK(minimax_err < 3.8e-6);
    CHECK(minimax_largest <= 1.0f);
}

TEST_CASE("Compressor stays within -120 dBFS of libm without fused multiply-add",
          "[signal][compressor][no-contract]") {
    for (const auto ballistics :
         {CompressorBallistics::branching, CompressorBallistics::decoupled}) {
        INFO("ballistics " << static_cast<int>(ballistics));
        Compressor c;
        c.set_sample_rate(48000.0f);
        Compressor::Params p;
        p.threshold_db = -24.0f;
        p.ratio = 6.0f;
        p.attack_ms = 2.0f;
        p.release_ms = 80.0f;
        p.knee_db = 6.0f;
        p.makeup_db = 3.0f;
        c.set_params(p);
        c.set_ballistics(ballistics);
        std::vector<float> input(16384);
        for (std::size_t i = 0; i < input.size(); ++i) {
            const float level = (i / 2048) % 2 == 0 ? 1.0f : 0.05f;
            input[i] = level * std::sin(0.0287f * static_cast<float>(i));
        }
        auto output = input;
        c.process(output.data(), static_cast<int>(output.size()));

        // The same algorithm with libm conversions. The ballistic coefficients
        // come from the compressor: a constant-folded std::exp here can differ
        // from the runtime libm result by an ulp, which the slow release then
        // carries for thousands of samples.
        const float attack = c.attack_coefficient();
        const float release = c.release_coefficient();
        const float slope = 1.0f / 6.0f - 1.0f;
        float env = 0.0f;
        float hold = 0.0f;
        double worst = 0.0;
        for (std::size_t i = 0; i < input.size(); ++i) {
            const float in_db = 20.0f * std::log10(std::max(std::abs(input[i]), 1e-10f));
            float g = 0.0f;
            if (in_db > -24.0f + 3.0f)
                g = (in_db + 24.0f) * slope;
            else if (in_db >= -24.0f - 3.0f) {
                const float x = in_db + 24.0f + 3.0f;
                g = slope * x * x / 12.0f;
            }
            if (ballistics == CompressorBallistics::decoupled) {
                // Peak hold, then attack smoothing.
                const float released = hold + release * (g - hold);
                hold = g < released ? g : released;
                env = env + attack * (hold - env);
            } else {
                env = env + (g < env ? attack : release) * (g - env);
            }
            const float ref = input[i] * std::pow(10.0f, (env + 3.0f) / 20.0f);
            worst = std::max(worst, std::abs(static_cast<double>(output[i]) - ref));
        }
        CHECK(worst < 1.0e-6);
    }
}
