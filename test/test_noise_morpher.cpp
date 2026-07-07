// NoiseMorpher — magnitude-locked random-phase noise resynthesis for
// transparent noise time-stretching.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/signal/noise_morpher.hpp>
#include <cmath>
#include <complex>
#include <vector>

using namespace pulp::signal;
using Catch::Matchers::WithinAbs;

namespace {
constexpr int kBins = 257;
std::vector<float> ramp_env(float scale) {
    std::vector<float> e(static_cast<size_t>(kBins));
    for (int k = 0; k < kBins; ++k) e[static_cast<size_t>(k)] = scale * (0.1f + k * 0.003f);
    return e;
}
} // namespace

TEST_CASE("NoiseMorpher output magnitude matches the envelope", "[signal][noise-morpher]") {
    NoiseMorpher nm;
    nm.prepare(kBins, 12345);
    auto env = ramp_env(1.0f);
    nm.push_envelope(env.data()); // first push seeds both endpoints
    std::vector<std::complex<float>> out(static_cast<size_t>(kBins));
    nm.synthesize(0.0f, out.data());
    for (int k = 0; k < kBins; ++k)
        REQUIRE_THAT(std::abs(out[static_cast<size_t>(k)]),
                     WithinAbs(env[static_cast<size_t>(k)], 1e-4f));
}

TEST_CASE("NoiseMorpher64 output magnitude matches a double envelope",
          "[signal][noise-morpher][f64]") {
    NoiseMorpher64 nm;
    nm.prepare(kBins, 12345);
    std::vector<double> env(static_cast<size_t>(kBins));
    for (int k = 0; k < kBins; ++k)
        env[static_cast<size_t>(k)] = 0.25 + static_cast<double>(k) * 0.001;
    nm.push_envelope(env.data());

    std::vector<std::complex<double>> out(static_cast<size_t>(kBins));
    nm.synthesize(0.0, out.data());
    for (int k = 0; k < kBins; k += 37)
        REQUIRE_THAT(std::abs(out[static_cast<size_t>(k)]),
                     WithinAbs(env[static_cast<size_t>(k)], 1e-10));
}

TEST_CASE("NoiseMorpher interpolates the envelope across frames", "[signal][noise-morpher]") {
    NoiseMorpher nm;
    nm.prepare(kBins, 999);
    auto a = ramp_env(1.0f);
    auto b = ramp_env(3.0f);
    nm.push_envelope(a.data());  // from = a, to = a
    nm.advance();                // from = a
    nm.push_envelope(b.data());  // to = b
    std::vector<std::complex<float>> out(static_cast<size_t>(kBins));
    nm.synthesize(0.5f, out.data());
    // Halfway: magnitude is the average of a and b.
    for (int k = 10; k < kBins; k += 32) {
        const float expect = 0.5f * (a[static_cast<size_t>(k)] + b[static_cast<size_t>(k)]);
        REQUIRE_THAT(std::abs(out[static_cast<size_t>(k)]), WithinAbs(expect, 1e-4f));
    }
}

TEST_CASE("NoiseMorpher phases are random and decorrelated", "[signal][noise-morpher]") {
    NoiseMorpher nm;
    nm.prepare(kBins, 42);
    auto env = ramp_env(1.0f);
    nm.push_envelope(env.data());
    std::vector<std::complex<float>> f1(static_cast<size_t>(kBins)), f2(static_cast<size_t>(kBins));
    nm.synthesize(0.0f, f1.data());
    nm.synthesize(0.0f, f2.data());
    // Successive synthesis frames use fresh random phase → different.
    int differ = 0;
    for (int k = 1; k < kBins; ++k)
        if (std::abs(std::arg(f1[static_cast<size_t>(k)]) - std::arg(f2[static_cast<size_t>(k)])) > 1e-3f)
            ++differ;
    REQUIRE(differ > kBins / 2);
    // Phase distribution is broad (not a constant) within one frame.
    float min_ph = 10.0f, max_ph = -10.0f;
    for (int k = 1; k < kBins; ++k) {
        const float p = std::arg(f1[static_cast<size_t>(k)]);
        min_ph = std::min(min_ph, p);
        max_ph = std::max(max_ph, p);
    }
    REQUIRE(max_ph - min_ph > 3.0f); // spans most of [-pi, pi)
}

TEST_CASE("NoiseMorpher is deterministic for a fixed seed", "[signal][noise-morpher]") {
    auto run = [] {
        NoiseMorpher nm;
        nm.prepare(kBins, 7);
        auto env = ramp_env(1.0f);
        nm.push_envelope(env.data());
        std::vector<std::complex<float>> out(static_cast<size_t>(kBins));
        nm.synthesize(0.0f, out.data());
        return out;
    };
    auto a = run();
    auto b = run();
    for (int k = 0; k < kBins; ++k) {
        REQUIRE(a[static_cast<size_t>(k)].real() == b[static_cast<size_t>(k)].real());
        REQUIRE(a[static_cast<size_t>(k)].imag() == b[static_cast<size_t>(k)].imag());
    }
}
